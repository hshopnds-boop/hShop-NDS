#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dswifi9.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <ctype.h>

// ======================== MAXMOD ========================
#include <maxmod9.h>
#include "soundbank.h"
#include "soundbank_bin.h"
// ========================================================

// Images
extern const u16 top_bg_bin[];
extern const u16 bottom_bg_bin[];

// Configuration
#define API_HOST "my ip"
#define API_PORT 80
#define API_PATH "/api/titles"
#define MAX_TITLES 100
#define JSON_BUFFER_SIZE 20480
#define CHUNK_SIZE (16 * 1024 * 1024)

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

// Structure for storing title information
typedef struct {
    char name[128];
    char id[32];
    char url[256];
    char size[32];
    int size_mb;
    char platform[16];
    bool is_compatible;
    bool visible; // Nouveau: pour le filtrage
} Title;

// Global variables
Title titles[MAX_TITLES];
int title_count = 0;
int selected_index = 0;
int scroll_offset = 0;
bool download_cancelled = false;
mm_sfxhand bgm_handle = 0;

// Variables pour la recherche
bool search_mode = false;
char search_query[64] = "";
int filtered_count = 0;

static char json_buffer[JSON_BUFFER_SIZE];
PrintConsole topScreen;
PrintConsole bottomScreen;

void cleanup_wifi_buffers(void) {
    swiWaitForVBlank();
}

// Initialize screens with background images
void init_screens(void) {
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bg_haut = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 1, 0);
    dmaCopy(top_bg_bin, bgGetGfxPtr(bg_haut), 256*192*2);
    consoleInit(&topScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 2, 0, true, true);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bg_bas = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 1, 0);
    dmaCopy(bottom_bg_bin, bgGetGfxPtr(bg_bas), 256*192*2);
    consoleInit(&bottomScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 2, 0, false, true);

    BG_PALETTE[255] = RGB15(0, 0, 0);
    BG_PALETTE[0] = 0;
    BG_PALETTE_SUB[255] = RGB15(0, 0, 0);
    BG_PALETTE_SUB[0] = 0;

    consoleSetWindow(&topScreen, 0, 0, 32, 24);
    consoleSetWindow(&bottomScreen, 0, 0, 32, 24);
}

void print_centered(PrintConsole* console, int y, const char* text) {
    consoleSelect(console);
    int len = strlen(text);
    int x = (32 - len) / 2;
    if (x < 0) x = 0;
    iprintf("\x1b[%d;%dH%s", y, x, text);
}

void print_centered_bold(PrintConsole* console, int y, const char* text) {
    consoleSelect(console);
    int len = strlen(text);
    int x = (32 - len) / 2;
    if (x < 0) x = 0;
    iprintf("\x1b[%d;%dH\x1b[1m%s\x1b[0m", y, x, text);
}

void print_wrapped(PrintConsole* console, int start_y, const char* text, int max_width) {
    consoleSelect(console);
    char buffer[128];
    strncpy(buffer, text, 127);
    buffer[127] = '\0';
    
    int len = strlen(buffer);
    int line = 0;
    int pos = 0;
    
    while (pos < len && line < 3) {
        int end_pos = pos + max_width;
        if (end_pos >= len) {
            iprintf("\x1b[%d;1H%s", start_y + line, buffer + pos);
            break;
        }
        
        int cut_pos = end_pos;
        for (int i = end_pos; i > pos; i--) {
            if (buffer[i] == ' ') {
                cut_pos = i;
                break;
            }
        }
        
        char line_buffer[33];
        int line_len = cut_pos - pos;
        if (line_len > 32) line_len = 32;
        strncpy(line_buffer, buffer + pos, line_len);
        line_buffer[line_len] = '\0';
        
        iprintf("\x1b[%d;1H%s", start_y + line, line_buffer);
        pos = cut_pos + 1;
        line++;
    }
}

// Fonction pour nettoyer le nom de fichier
void sanitize_filename(const char* input, char* output, int max_len) {
    int j = 0;
    for (int i = 0; input[i] != '\0' && j < max_len - 1; i++) {
        char c = input[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            output[j++] = c;
        } else if (c == ' ') {
            output[j++] = ' ';
        }
    }
    output[j] = '\0';
}

// Fonction de recherche insensible à la casse
bool contains_ignore_case(const char* haystack, const char* needle) {
    if (strlen(needle) == 0) return true;
    
    char hay_lower[128], need_lower[64];
    
    for (int i = 0; haystack[i] && i < 127; i++) {
        hay_lower[i] = tolower(haystack[i]);
    }
    hay_lower[strlen(haystack)] = '\0';
    
    for (int i = 0; needle[i] && i < 63; i++) {
        need_lower[i] = tolower(needle[i]);
    }
    need_lower[strlen(needle)] = '\0';
    
    return strstr(hay_lower, need_lower) != NULL;
}

// Applique le filtre de recherche
void apply_search_filter(void) {
    filtered_count = 0;
    
    if (strlen(search_query) == 0) {
        // Pas de recherche, tous les jeux sont visibles
        for (int i = 0; i < title_count; i++) {
            titles[i].visible = true;
        }
        filtered_count = title_count;
    } else {
        // Filtre selon la recherche
        for (int i = 0; i < title_count; i++) {
            titles[i].visible = contains_ignore_case(titles[i].name, search_query);
            if (titles[i].visible) filtered_count++;
        }
    }
    
    // Réajuste l'index sélectionné
    if (filtered_count > 0) {
        // Trouve le premier jeu visible
        for (int i = 0; i < title_count; i++) {
            if (titles[i].visible) {
                selected_index = i;
                break;
            }
        }
        scroll_offset = 0;
    }
}

// Obtient l'index du Nième jeu visible
int get_visible_index(int n) {
    int count = 0;
    for (int i = 0; i < title_count; i++) {
        if (titles[i].visible) {
            if (count == n) return i;
            count++;
        }
    }
    return -1;
}

// Obtient la position d'un index dans la liste filtrée
int get_filtered_position(int index) {
    int pos = 0;
    for (int i = 0; i < index; i++) {
        if (titles[i].visible) pos++;
    }
    return pos;
}

// Affiche le clavier et retourne true si validé, false si annulé
bool show_keyboard(char* buffer, int max_len) {
    Keyboard* kbd = keyboardDemoInit();
    kbd->OnKeyPressed = NULL;
    
    keyboardShow();
    
    bool done = false;
    bool cancelled = false;
    
    while (!done) {
        swiWaitForVBlank();
        scanKeys();
        
        int keys = keysDown();
        
        if (keys & KEY_B) {
            cancelled = true;
            done = true;
        }
        
        int key = keyboardUpdate();
        
        if (key > 0) {
            if (key == DVK_ENTER) {
                done = true;
            } else if (key == DVK_BACKSPACE) {
                int len = strlen(buffer);
                if (len > 0) {
                    buffer[len - 1] = '\0';
                }
            } else if (strlen(buffer) < max_len - 1) {
                int len = strlen(buffer);
                buffer[len] = (char)key;
                buffer[len + 1] = '\0';
            }
        }
    }
    
    keyboardHide();
    
    return !cancelled;
}

// Barre de progression
void draw_progress_bar(int percent) {
    consoleSelect(&bottomScreen);
    
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    int bar_width = 26;
    int filled = (bar_width * percent) / 100;
    
    iprintf("\x1b[10;3H");
    iprintf("[");
    
    iprintf("\x1b[32m");
    for (int i = 0; i < filled; i++) {
        iprintf("\xDB");
    }
    iprintf("\x1b[0m");
    
    iprintf("\x1b[30m");
    for (int i = filled; i < bar_width; i++) {
        iprintf("\xB0");
    }
    iprintf("\x1b[0m");
    
    iprintf("]");
    
    char percent_str[16];
    snprintf(percent_str, 16, "%d%%", percent);
    print_centered(&bottomScreen, 11, percent_str);
}

void parse_json_titles(char* json_data) {
    title_count = 0;
    char* ptr = json_data;
    
    ptr = strstr(ptr, "\"titles\"");
    if (!ptr) return;
    
    ptr = strchr(ptr, '[');
    if (!ptr) return;
    ptr++;
    
    while ((ptr = strchr(ptr, '{')) != NULL && title_count < MAX_TITLES) {
        ptr++;
        char* obj_end = strchr(ptr, '}');
        if (!obj_end) break;
        
        char* name_ptr = strstr(ptr, "\"name\"");
        if (name_ptr && name_ptr < obj_end) {
            name_ptr = strchr(name_ptr, ':');
            if (name_ptr) {
                name_ptr = strchr(name_ptr, '"');
                if (name_ptr) {
                    name_ptr++;
                    char* name_end = strchr(name_ptr, '"');
                    if (name_end && name_end < obj_end) {
                        int len = name_end - name_ptr;
                        if (len > 127) len = 127;
                        strncpy(titles[title_count].name, name_ptr, len);
                        titles[title_count].name[len] = '\0';
                    }
                }
            }
        }
        
        char* id_ptr = strstr(ptr, "\"id\"");
        if (id_ptr && id_ptr < obj_end) {
            id_ptr = strchr(id_ptr, ':');
            if (id_ptr) {
                id_ptr = strchr(id_ptr, '"');
                if (id_ptr) {
                    id_ptr++;
                    char* id_end = strchr(id_ptr, '"');
                    if (id_end && id_end < obj_end) {
                        int len = id_end - id_ptr;
                        if (len > 31) len = 31;
                        strncpy(titles[title_count].id, id_ptr, len);
                        titles[title_count].id[len] = '\0';
                    }
                }
            }
        }
        
        char* url_ptr = strstr(ptr, "\"url\"");
        if (url_ptr && url_ptr < obj_end) {
            url_ptr = strchr(url_ptr, ':');
            if (url_ptr) {
                url_ptr = strchr(url_ptr, '"');
                if (url_ptr) {
                    url_ptr++;
                    char* url_end = strchr(url_ptr, '"');
                    if (url_end && url_end < obj_end) {
                        int len = url_end - url_ptr;
                        if (len > 255) len = 255;
                        strncpy(titles[title_count].url, url_ptr, len);
                        titles[title_count].url[len] = '\0';
                    }
                }
            }
        }
        
        titles[title_count].size_mb = 0;
        char* size_ptr = strstr(ptr, "\"size\"");
        if (size_ptr && size_ptr < obj_end) {
            size_ptr = strchr(size_ptr, ':');
            if (size_ptr) {
                size_ptr++;
                while (*size_ptr == ' ') size_ptr++;
                char size_buf[32];
                int i = 0;
                while (*size_ptr >= '0' && *size_ptr <= '9' && i < 31) {
                    size_buf[i++] = *size_ptr++;
                }
                size_buf[i] = '\0';
                long bytes = atol(size_buf);
                titles[title_count].size_mb = bytes / (1024 * 1024);
                snprintf(titles[title_count].size, 32, "%dMB", titles[title_count].size_mb);
            }
        } else {
            strcpy(titles[title_count].size, "?");
        }
        
        char* platform_ptr = strstr(ptr, "\"platform\"");
        if (platform_ptr && platform_ptr < obj_end) {
            platform_ptr = strchr(platform_ptr, ':');
            if (platform_ptr) {
                platform_ptr = strchr(platform_ptr, '"');
                if (platform_ptr) {
                    platform_ptr++;
                    char* platform_end = strchr(platform_ptr, '"');
                    if (platform_end && platform_end < obj_end) {
                        int len = platform_end - platform_ptr;
                        if (len > 15) len = 15;
                        strncpy(titles[title_count].platform, platform_ptr, len);
                        titles[title_count].platform[len] = '\0';
                    }
                }
            }
        } else {
            strcpy(titles[title_count].platform, "NDS");
        }
        
        titles[title_count].is_compatible = true;
        titles[title_count].visible = true;
        title_count++;
        
        ptr = obj_end + 1;
    }
    
    filtered_count = title_count;
}

bool connect_wifi(void) {
    consoleSelect(&topScreen);
    consoleClear();
    print_centered(&topScreen, 10, "Connecting to WiFi...");
    
    if (!Wifi_InitDefault(WFC_CONNECT)) {
        print_centered(&topScreen, 12, "WiFi Error");
        print_centered(&topScreen, 13, "Check your settings");
        return false;
    }
    
    print_centered(&topScreen, 12, "WiFi connected!");
    
    struct in_addr ip, gw, sn, dns1, dns2;
    ip = Wifi_GetIPInfo(&gw, &sn, &dns1, &dns2);
    char ip_str[32];
    snprintf(ip_str, 32, "IP: %s", inet_ntoa(ip));
    print_centered(&topScreen, 13, ip_str);
    
    return true;
}

char* http_get(const char* host, const char* path) {
    consoleSelect(&topScreen);
    consoleClear();
    print_centered(&topScreen, 10, "Connecting to server...");
    
    struct hostent* server = gethostbyname(host);
    if (!server) {
        print_centered(&topScreen, 12, "DNS Error");
        return NULL;
    }
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        print_centered(&topScreen, 12, "Socket Error");
        return NULL;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_addr.sin_port = htons(API_PORT);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        print_centered(&topScreen, 12, "Connection Error");
        closesocket(sock);
        return NULL;
    }
    
    print_centered(&topScreen, 12, "Receiving data...");
    
    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: NintendoDS-Client/1.0\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n", path, host);
    
    send(sock, request, strlen(request), 0);
    
    memset(json_buffer, 0, JSON_BUFFER_SIZE);
    int total = 0;
    int bytes;
    
    while ((bytes = recv(sock, json_buffer + total, 512, 0)) > 0) {
        total += bytes;
        if (total >= JSON_BUFFER_SIZE - 1) {
            break;
        }
    }
    json_buffer[total] = '\0';
    
    closesocket(sock);
    
    char* json_start = strstr(json_buffer, "\r\n\r\n");
    if (json_start) {
        json_start += 4;
        return json_start;
    }
    
    return NULL;
}

bool download_file_chunked(const char* url, const char* filepath, long expected_size) {
    download_cancelled = false;
    
    consoleSelect(&topScreen);
    consoleClear();
    print_centered_bold(&topScreen, 8, "DOWNLOADING");
    
    consoleSelect(&bottomScreen);
    consoleClear();
    print_centered(&bottomScreen, 6, "Download in progress...");
    print_centered(&bottomScreen, 14, "Hold B to cancel");
    
    char host[128];
    char path[256];
    
    const char* host_start = strstr(url, "://");
    if (!host_start) return false;
    host_start += 3;
    
    const char* path_start = strchr(host_start, '/');
    if (!path_start) return false;
    
    int host_len = path_start - host_start;
    if (host_len >= 128) host_len = 127;
    strncpy(host, host_start, host_len);
    host[host_len] = '\0';
    
    strncpy(path, path_start, 255);
    path[255] = '\0';
    
    FILE* file = fopen(filepath, "wb");
    if (!file) {
        print_centered(&bottomScreen, 8, "Error creating file");
        return false;
    }
    
    unsigned long offset = 0;
    unsigned long total_downloaded = 0;
    int chunk_number = 1;
    
    while (!download_cancelled) {
        scanKeys();
        int keys = keysHeld();
        if (keys & KEY_B) {
            download_cancelled = true;
            fclose(file);
            remove(filepath);
            consoleSelect(&bottomScreen);
            consoleClear();
            print_centered(&bottomScreen, 10, "Download cancelled");
            return false;
        }
        
        struct hostent* server = gethostbyname(host);
        if (!server) {
            fclose(file);
            return false;
        }
        
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            fclose(file);
            return false;
        }
        
        int sock_buf_size = 16384;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sock_buf_size, sizeof(sock_buf_size));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sock_buf_size, sizeof(sock_buf_size));
        
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
        server_addr.sin_port = htons(80);
        
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            closesocket(sock);
            fclose(file);
            return false;
        }
        
        char request[512];
        snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Range: bytes=%lu-%lu\r\n"
            "Connection: close\r\n"
            "\r\n", path, host, offset, offset + CHUNK_SIZE - 1);
        
        send(sock, request, strlen(request), 0);
        
        char* buffer = (char*)malloc(4096);
        if (!buffer) {
            closesocket(sock);
            fclose(file);
            return false;
        }
        
        int bytes;
        unsigned long chunk_bytes = 0;
        bool headers_skipped = false;
        int flush_counter = 0;
        
        while ((bytes = recv(sock, buffer, 4096, 0)) > 0) {
            scanKeys();
            if (keysHeld() & KEY_B) {
                download_cancelled = true;
                break;
            }
            
            if (!headers_skipped) {
                char* data_start = strstr(buffer, "\r\n\r\n");
                if (data_start) {
                    data_start += 4;
                    int data_len = bytes - (data_start - buffer);
                    fwrite(data_start, 1, data_len, file);
                    chunk_bytes += data_len;
                    total_downloaded += data_len;
                    headers_skipped = true;
                }
            } else {
                fwrite(buffer, 1, bytes, file);
                chunk_bytes += bytes;
                total_downloaded += bytes;
            }
            
            flush_counter++;
            if (flush_counter >= 32) {
                fflush(file);
                cleanup_wifi_buffers();
                flush_counter = 0;
            }
            
            if (expected_size > 0) {
                long long downloaded_ll = (long long)total_downloaded;
                long long expected_ll = (long long)expected_size;
                int percent = (int)((downloaded_ll * 100LL) / expected_ll);
                
                if (percent > 100) percent = 100;
                if (percent < 0) percent = 0;
                
                draw_progress_bar(percent);
                
                char info[32];
                snprintf(info, 32, "%lu / %ld MB", 
                    total_downloaded / (1024 * 1024), 
                    expected_size / (1024 * 1024));
                print_centered(&bottomScreen, 8, info);
            }
        }
        
        free(buffer);
        closesocket(sock);
        
        if (download_cancelled) {
            fclose(file);
            remove(filepath);
            consoleSelect(&bottomScreen);
            consoleClear();
            print_centered(&bottomScreen, 10, "Download cancelled");
            return false;
        }
        
        if (chunk_bytes == 0) {
            break;
        }
        
        offset += chunk_bytes;
        chunk_number++;
        
        fflush(file);
        fsync(fileno(file));
        
        if (chunk_bytes < CHUNK_SIZE) {
            break;
        }
    }
    
    fclose(file);
    
    if (!download_cancelled) {
        consoleSelect(&bottomScreen);
        consoleClear();
        print_centered(&bottomScreen, 10, "Download complete!");
        draw_progress_bar(100);
    }
    
    return !download_cancelled;
}

int last_displayed_index = -1;

void display_ui(void) {
    consoleSelect(&topScreen);
    consoleClear();
    
    if (search_mode) {
        print_centered_bold(&topScreen, 0, "SEARCH MODE");
        char search_display[32];
        snprintf(search_display, 32, "Query: %s", search_query);
        print_centered(&topScreen, 1, search_display);
    } else {
        print_centered_bold(&topScreen, 0, "hShop NDS");
        char count_str[32];
        if (strlen(search_query) > 0) {
            snprintf(count_str, 32, "%d/%d games", filtered_count, title_count);
        } else {
            snprintf(count_str, 32, "%d games available", title_count);
        }
        print_centered(&topScreen, 1, count_str);
    }
    
    consoleSelect(&topScreen);
    iprintf("\x1b[2;0H________________________________");
    
    int visible_count = 20;
    int visible_pos = get_filtered_position(selected_index);
    int start = scroll_offset;
    int end = start + visible_count;
    
    int displayed = 0;
    int visible_counter = 0;
    
    for (int i = 0; i < title_count && displayed < visible_count; i++) {
        if (!titles[i].visible) continue;
        
        if (visible_counter >= start && visible_counter < end) {
            char display_line[33];
            char short_name[26];
            strncpy(short_name, titles[i].name, 23);
            short_name[23] = '\0';
            if (strlen(titles[i].name) > 23) {
                strcat(short_name, "...");
            }
            
            if (i == selected_index) {
                snprintf(display_line, 33, "\x1b[1m> %-26s\x1b[0m", short_name);
            } else {
                snprintf(display_line, 33, " %-26s", short_name);
            }
            
            consoleSelect(&topScreen);
            iprintf("\x1b[%d;0H%s", displayed + 3, display_line);
            displayed++;
        }
        
        visible_counter++;
    }
    
    if (last_displayed_index != selected_index) {
        consoleSelect(&bottomScreen);
        consoleClear();
        print_centered_bold(&bottomScreen, 0, "GAME DETAILS");
        
        consoleSelect(&bottomScreen);
        iprintf("\x1b[1;0H________________________________");
        
        consoleSelect(&bottomScreen);
        iprintf("\x1b[3;1H\x1b[1m");
        print_wrapped(&bottomScreen, 3, titles[selected_index].name, 30);
        
        consoleSelect(&bottomScreen);
        iprintf("\x1b[0m");
        
        consoleSelect(&bottomScreen);
        iprintf("\x1b[8;2HPlatform: \x1b[1m%s\x1b[0m", titles[selected_index].platform);
        
        consoleSelect(&bottomScreen);
        iprintf("\x1b[9;2HSize: \x1b[1m%s\x1b[0m", titles[selected_index].size);
        
        consoleSelect(&bottomScreen);
        iprintf("\x1b[20;0H________________________________");
        
        if (search_mode) {
            print_centered(&bottomScreen, 21, "X: Enter search");
            print_centered(&bottomScreen, 22, "B: Exit search");
        } else {
            print_centered(&bottomScreen, 21, "UP/DOWN: Navigate");
            print_centered(&bottomScreen, 22, "A: Download | X: Search");
            print_centered(&bottomScreen, 23, "B: Exit");
        }
        
        last_displayed_index = selected_index;
    }
}

int main(void) {
    init_screens();
    
    consoleSelect(&topScreen);
    print_centered_bold(&topScreen, 10, "hShop NDS");
    print_centered(&topScreen, 11, "Initializing...");
    
    if (!fatInitDefault()) {
        print_centered(&topScreen, 13, "Error: SD Card");
        while(1) {
            swiWaitForVBlank();
        }
    }
    
    // ======================== MAXMOD INIT ========================
    mmInitDefaultMem((mm_addr)soundbank_bin);
    mmSetModuleVolume(1024);
    mmSetJingleVolume(1024);
    mmLoadEffect(SFX_BGM);
    
    mm_sound_effect bgm_sound = {
        { SFX_BGM },
        (int)(1.0f * (1<<10)),
        0,
        255,
        128,
    };
    
    bgm_handle = mmEffectEx(&bgm_sound);
    // =============================================================
    
    if (!connect_wifi()) {
        print_centered(&bottomScreen, 10, "Press A to exit");
        while(1) {
            scanKeys();
            if (keysDown() & KEY_A) break;
            swiWaitForVBlank();
        }
        return 0;
    }
    
    consoleSelect(&topScreen);
    consoleClear();
    print_centered(&topScreen, 10, "Loading games...");
    
    char* json_data = http_get(API_HOST, API_PATH);
    if (!json_data) {
        consoleSelect(&topScreen);
        consoleClear();
        print_centered(&topScreen, 10, "Network Error");
        print_centered(&topScreen, 11, "Check server");
        print_centered(&bottomScreen, 10, "Press A to exit");
        while(1) {
            scanKeys();
            if (keysDown() & KEY_A) break;
            swiWaitForVBlank();
        }
        return 0;
    }
    
    parse_json_titles(json_data);
    
    if (title_count == 0) {
        consoleSelect(&topScreen);
        consoleClear();
        print_centered(&topScreen, 10, "No games found");
        print_centered(&bottomScreen, 10, "Press A to exit");
        while(1) {
            scanKeys();
            if (keysDown() & KEY_A) break;
            swiWaitForVBlank();
        }
        return 0;
    }
    
    while (1) {
        display_ui();
        
        scanKeys();
        int keys = keysDown();
        
        // Mode recherche
        if (keys & KEY_X && !search_mode) {
            search_mode = true;
            
            // Affiche l'écran de recherche
            consoleSelect(&topScreen);
            consoleClear();
            print_centered_bold(&topScreen, 8, "SEARCH");
            print_centered(&topScreen, 10, "Enter search query:");
            print_centered(&topScreen, 14, "Press B to cancel");
            
            consoleSelect(&bottomScreen);
            consoleClear();
            
            // Affiche le clavier
            if (show_keyboard(search_query, sizeof(search_query))) {
                // Recherche validée
                apply_search_filter();
                
                if (filtered_count == 0) {
                    consoleSelect(&topScreen);
                    consoleClear();
                    print_centered(&topScreen, 10, "No games found");
                    print_centered(&topScreen, 12, "Press A to continue");
                    
                    while(1) {
                        scanKeys();
                        if (keysDown() & KEY_A) break;
                        swiWaitForVBlank();
                    }
                    
                    // Réinitialise la recherche
                    search_query[0] = '\0';
                    apply_search_filter();
                }
            } else {
                // Recherche annulée
                search_query[0] = '\0';
                apply_search_filter();
            }
            
            search_mode = false;
            last_displayed_index = -1;
            continue;
        }
        
        // Quitter le mode recherche avec B
        if (keys & KEY_B && strlen(search_query) > 0) {
            search_query[0] = '\0';
            apply_search_filter();
            last_displayed_index = -1;
            continue;
        }
        
        // Navigation
        if (keys & KEY_DOWN) {
            // Trouve le prochain jeu visible
            int current_pos = get_filtered_position(selected_index);
            if (current_pos < filtered_count - 1) {
                for (int i = selected_index + 1; i < title_count; i++) {
                    if (titles[i].visible) {
                        selected_index = i;
                        break;
                    }
                }
                
                int visible_pos = get_filtered_position(selected_index);
                if (visible_pos >= scroll_offset + 20) {
                    scroll_offset++;
                }
            }
        }
        
        if (keys & KEY_UP) {
            // Trouve le jeu visible précédent
            int current_pos = get_filtered_position(selected_index);
            if (current_pos > 0) {
                for (int i = selected_index - 1; i >= 0; i--) {
                    if (titles[i].visible) {
                        selected_index = i;
                        break;
                    }
                }
                
                int visible_pos = get_filtered_position(selected_index);
                if (visible_pos < scroll_offset) {
                    scroll_offset--;
                }
            }
        }
        
        // Quitter l'application
        if (keys & KEY_B && strlen(search_query) == 0) {
            break;
        }
        
        // Télécharger
        if (keys & KEY_A) {
            char filepath[256];
            char clean_name[128];
            
            sanitize_filename(titles[selected_index].name, clean_name, sizeof(clean_name));
            
            const char* extension;
            if (strcmp(titles[selected_index].platform, "GBA") == 0) {
                extension = ".gba";
            } else {
                extension = ".nds";
            }
            
            snprintf(filepath, sizeof(filepath), "/%s%s", clean_name, extension);
            
            long expected_size = titles[selected_index].size_mb * 1024L * 1024L;
            
            if (download_file_chunked(titles[selected_index].url, filepath, expected_size)) {
                consoleSelect(&bottomScreen);
                print_centered(&bottomScreen, 12, "File saved successfully!");
                
                while(1) {
                    scanKeys();
                    if (keysDown() & KEY_A) break;
                    swiWaitForVBlank();
                }
            } else {
                while(1) {
                    scanKeys();
                    if (keysDown() & KEY_A) break;
                    swiWaitForVBlank();
                }
            }
            
            last_displayed_index = -1;
        }
        
        swiWaitForVBlank();
    }
    
    consoleSelect(&topScreen);
    consoleClear();
    print_centered(&topScreen, 10, "Thank you for using");
    print_centered(&topScreen, 11, "hShop NDS!");
    
    for(int i = 0; i < 60; i++) {
        swiWaitForVBlank();
    }
    
    return 0;
}