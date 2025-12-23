// mapviewer_raylib.c
// Raylib single-file OSM tile viewer with libcurl + pthreads.
// Features: pan (mouse drag), wheel zoom, on-disk cache, background tile downloads.
//
// Build:
// gcc -O2 mapviewer_raylib.c -o mapviewer_raylib -lraylib -lcurl -lpthread -lm

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <raylib.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "PSXMAP.h"

static TexNode *tex_head = NULL;
static pthread_mutex_t tex_mutex = PTHREAD_MUTEX_INITIALIZER;

static DLNode *dl_head = NULL;
static pthread_mutex_t dl_mutex = PTHREAD_MUTEX_INITIALIZER;

// Map state: center in fractional tile coords and zoom
static double center_tx = 0.0, center_ty = 0.0;
static int zoom_level = 13;

// Helpers: tile math
static inline double lon_to_xtile_d(double lon, int z)
{
    double result = (1.0 + lon / M_PI) / 2.0 * (1 << z);
    return result;
}
static inline double lat_to_ytile_d(double lat, int z)
{
    // double lat_rad = lat * M_PI / 180.0;
    return (1.0 - log(tan(lat) + 1.0 / cos(lat)) / M_PI) / 2.0 * (1 << z);
}

int umain(void)
{
    size_t bufmain_remain = sizeof(bufmain) - bufmain_used;
    if (bufmain_remain == 0) {
        printf("Main socket line exceeded buffer length! Discarding input");
        bufmain_used = 0;
        return 0;
    }

    int nbread = recv(socketID, (char *)&bufmain[bufmain_used], bufmain_remain, 0);

    bufmain_used += nbread;

    /* Scan for newlines in the line buffer; we're careful here to deal with
     * embedded \0s an evil server may send, as well as only processing lines
     * that are complete.
     */
    char *line_start = bufmain;
    char *line_end;
    while ((line_end = (char *)memchr((void *)line_start, '\n',
                                      bufmain_used - (line_start - bufmain)))) {
        *line_end = 0;
        if (strstr(line_start, "Qs121")) {
            decode_pos(line_start);
        }
        //log_position();

        line_start = line_end + 1;
    }
    /* Shift buffer down so the unprocessed data is at the start */
    bufmain_used -= (line_start - bufmain);
    memmove(bufmain, line_start, bufmain_used);
    return nbread;
}
void *ptUmain(void *thread_param)
{
    (void)(thread_param);
    while (1) {
        umain();
    }
    printf("Exiting ptUmain\n");
    return NULL;
}
// mkdir -p style
void mkdir_p(const char *path)
{
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len == 0)
        return;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void ensure_cache_dirs(int z, int x)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%d", CACHE_DIR, z);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/%d/%d", CACHE_DIR, z, x);
    mkdir_p(path);
}

int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

// Download queue helpers
void add_downloading(const char *k)
{
    pthread_mutex_lock(&dl_mutex);
    DLNode *n = malloc(sizeof(DLNode));
    n->key = strdup(k);
    n->next = dl_head;
    dl_head = n;
    pthread_mutex_unlock(&dl_mutex);
}
int is_downloading(const char *k)
{
    int found = 0;
    pthread_mutex_lock(&dl_mutex);
    for (DLNode *n = dl_head; n; n = n->next) {
        if (strcmp(n->key, k) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&dl_mutex);
    return found;
}
void remove_downloading(const char *k)
{
    pthread_mutex_lock(&dl_mutex);
    DLNode **pp = &dl_head;
    while (*pp) {
        if (strcmp((*pp)->key, k) == 0) {
            DLNode *t = *pp;
            *pp = t->next;
            free(t->key);
            free(t);
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&dl_mutex);
}

// Texture cache helpers
Texture2D *find_texture_cache(int z, int x, int y)
{
    pthread_mutex_lock(&tex_mutex);
    for (TexNode *n = tex_head; n; n = n->next) {
        if (n->z == z && n->x == x && n->y == y) {
            Texture2D *t = &n->tex;
            pthread_mutex_unlock(&tex_mutex);
            return t;
        }
    }
    pthread_mutex_unlock(&tex_mutex);
    return NULL;
}
void add_texture_cache(int z, int x, int y, Texture2D tex)
{
    TexNode *n = malloc(sizeof(TexNode));
    n->z = z;
    n->x = x;
    n->y = y;
    n->tex = tex;
    pthread_mutex_lock(&tex_mutex);
    n->next = tex_head;
    tex_head = n;
    pthread_mutex_unlock(&tex_mutex);
}
void free_all_textures(void)
{
    pthread_mutex_lock(&tex_mutex);
    TexNode *n = tex_head;
    while (n) {
        UnloadTexture(n->tex);
        TexNode *nx = n->next;
        free(n);
        n = nx;
    }
    tex_head = NULL;
    pthread_mutex_unlock(&tex_mutex);
}

// Background downloader arguments and thread
typedef struct DLArg {
    int z, x, y;
    char path[512];
    char url[256];
} DLArg;

size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *f = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, f);
}

void *download_tile_thread(void *arg)
{
    DLArg *d = (DLArg *)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        remove_downloading(d->path);
        free(d);
        return NULL;
    }

    ensure_cache_dirs(d->z, d->x);

    FILE *f = fopen(d->path, "wb");
    if (!f) {
        curl_easy_cleanup(curl);
        remove_downloading(d->path);
        free(d);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, d->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    fclose(f);

    if (res != CURLE_OK) {
        // remove incomplete file
        unlink(d->path);
    }

    curl_easy_cleanup(curl);
    remove_downloading(d->path);
    free(d);
    return NULL;
}

void request_tile(int z, int x, int y)
{
    int max = 1 << z;
    if (x < 0 || x >= max || y < 0 || y >= max) {
        return; // simple bounds
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%d/%d/%d.png", CACHE_DIR, z, x, y);
    if (file_exists(path))
        return;

    // if already downloading, skip
    if (is_downloading(path))
        return;

    // build URL (tile.openstreetmap.org)
    char url[256];
    snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);

    // spawn a thread to download
    DLArg *d = malloc(sizeof(DLArg));
    d->z = z;
    d->x = x;
    d->y = y;
    strncpy(d->path, path, sizeof(d->path));
    d->path[sizeof(d->path) - 1] = '\0';
    strncpy(d->url, url, sizeof(d->url));
    d->url[sizeof(d->url) - 1] = '\0';

    ensure_cache_dirs(z, x);
    add_downloading(path);

    pthread_t tid;
    pthread_create(&tid, NULL, download_tile_thread, d);
    pthread_detach(tid);
}

// Try to load tile texture if file exists and not already cached.
// Must be called on main thread (we use raylib loading functions).
Texture2D try_load_tile_texture(int z, int x, int y, int *loaded)
{
    *loaded = 0;
    Texture2D *cached = find_texture_cache(z, x, y);
    if (cached) {
        return *cached;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%d/%d/%d.png", CACHE_DIR, z, x, y);
    if (!file_exists(path)) {
        // return empty texture
        Texture2D empty = { 0 };
        return empty;
    }

    Image img = LoadImage(path); // raylib image loader
    if (img.data == NULL) {
        Texture2D empty = { 0 };
        return empty;
    }
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    add_texture_cache(z, x, y, tex);
    *loaded = 1;
    return tex;
}
int init_connect(void)
{
    struct sockaddr_in PSXMAIN;

    socketID = socket(AF_INET, SOCK_STREAM, 0);

    PSXMAIN.sin_family = AF_INET;
    PSXMAIN.sin_port = htons(10747);
    PSXMAIN.sin_addr.s_addr = inet_addr("127.0.0.1");
    /* Now connect to the server */
    if (connect(socketID, (struct sockaddr *)&PSXMAIN, sizeof(PSXMAIN)) < 0) {
        perror("ERROR connecting to main server");
        exit(1);
    }
    printf("Connected to PSX\n");
    return 0;
}

void decode_pos(char *s)
{
    double latitude, longitude;
    double heading;

    printf("q: %s\n", s);
    strtok(s + 6, ";"); //pitch
    strtok(NULL, ";"); // bank
    heading = strtof(strtok(NULL, ";"), NULL); //heading
    strtok(NULL, ";"); //altitude
    strtok(NULL, ";"); //TAS
    latitude = strtof(strtok(NULL, ";"), NULL);
    longitude = strtof(strtok(NULL, ";"), NULL);
    Pos.latitude = latitude;
    Pos.longitude = longitude;
    Pos.heading = heading;
}

int main(int argc, char **argv)
{
    // optional cmdline args: lat lon zoom
    pthread_t TPSX;
    init_connect();
    if (pthread_create(&TPSX, NULL, &ptUmain, NULL) != 0) {
        printf("Error creating thread Umain");
    }

    pthread_detach(TPSX);

    sleep(2);
    center_tx = lon_to_xtile_d(Pos.longitude, zoom_level);
    center_ty = lat_to_ytile_d(Pos.latitude, zoom_level);

    mkdir(CACHE_DIR, 0755); // ignore error if exists

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "PSXMAP");
    SetTargetFPS(60);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    bool dragging = false;
    Vector2 lastMouse = { 0, 0 };

    while (!WindowShouldClose()) {
        // Input handling --------------------------------
        Vector2 mouse = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            dragging = true;
            lastMouse = mouse;
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            dragging = false;
        }
        if (dragging && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            float dx = mouse.x - lastMouse.x;
            float dy = mouse.y - lastMouse.y;
            lastMouse = mouse;
            // translate pixel movement to fractional tile movement (inverse direction)
            double tiles_dx = -(double)dx / TILE_SIZE;
            double tiles_dy = -(double)dy / TILE_SIZE;
            center_tx += tiles_dx;
            center_ty += tiles_dy;
        }

        float wheel = GetMouseWheelMove(); // positive = up (zoom in)
        if (wheel > 0.0f && zoom_level < 19) {
            // zoom in: double tile coordinates (keeping geo center)
            center_tx = center_tx * 2.0;
            center_ty = center_ty * 2.0;
            zoom_level++;
        } else if (wheel < 0.0f && zoom_level > 1) {
            // zoom out: halve tile coordinates
            center_tx = center_tx / 2.0;
            center_ty = center_ty / 2.0;
            zoom_level--;
        }

        // Rendering -------------------------------------

        // tiles range to draw
        int tiles_w = (WINDOW_WIDTH / TILE_SIZE) + 3;
        int tiles_h = (WINDOW_HEIGHT / TILE_SIZE) + 3;

        // center tile indices (integer) and fractional parts
        center_tx = lon_to_xtile_d(Pos.longitude, zoom_level);
        center_ty = lat_to_ytile_d(Pos.latitude, zoom_level);
        double cx = center_tx;
        double cy = center_ty;
        int center_ix = (int)floor(cx);
        int center_iy = (int)floor(cy);
        double frac_x = cx - center_ix;
        double frac_y = cy - center_iy;

        double start_x = WINDOW_WIDTH / 2.0 - frac_x * TILE_SIZE;
        double start_y = WINDOW_HEIGHT / 2.0 - frac_y * TILE_SIZE;

        int half_w = tiles_w / 2;
        int half_h = tiles_h / 2;

        RenderTexture2D target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        BeginTextureMode(target);

        for (int dy = -half_h; dy <= half_h; dy++) {
            for (int dx = -half_w; dx <= half_w; dx++) {
                int tx = center_ix + dx;
                int ty = center_iy + dy;
                int z = zoom_level;

                int px = (int)round(start_x + dx * TILE_SIZE);
                int py = (int)round(start_y + dy * TILE_SIZE);

                // Request tile async (if missing)
                request_tile(z, tx, ty);

                // Try to load tile texture (main thread) if exists and not cached
                int loaded = 0;
                try_load_tile_texture(z, tx, ty, &loaded);
                Texture2D *cached = find_texture_cache(z, tx, ty);
                if (cached && cached->id != 0) {
                    DrawTextureEx(*cached, (Vector2){ (float)px, (float)py }, 0, 1.0f, WHITE);
                } else {
                    // placeholder
                    DrawRectangle(px, py, TILE_SIZE, TILE_SIZE, (Color){ 160, 160, 160, 255 });
                    DrawRectangleLines(px, py, TILE_SIZE, TILE_SIZE, (Color){ 100, 100, 100, 255 });
                }
            }
        }

        // draw crosshair at center
        DrawLine(WINDOW_WIDTH / 2 - 10, WINDOW_HEIGHT / 2, WINDOW_WIDTH / 2 + 10, WINDOW_HEIGHT / 2,
                 RED);
        DrawLine(WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2 - 10, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2 + 10,
                 RED);

        EndTextureMode();
        BeginDrawing();
        ClearBackground((Color){ 200, 200, 200, 255 });
        Rectangle source = {
            0, 0, (float)target.texture.width,
            -(float)target.texture.height // IMPORTANT: flip Y
        };
        Rectangle dest = { GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f,
                           (float)GetScreenWidth(), (float)GetScreenHeight() };

        Vector2 origin = { dest.width / 2.0f, dest.height / 2.0f };
        DrawTexturePro(target.texture, source, dest, origin, 360 - Pos.heading * 180.0 / M_PI,
                       WHITE);

        // draw status (zoom & center lat/lon)
        // compute center lat lon from tile coords
        double n = M_PI - 2.0 * M_PI * center_ty / (double)(1 << zoom_level);
        double lat = atan(0.5 * (exp(n) - exp(-n)));
        double lon = center_tx / (double)(1 << zoom_level) * 2 * M_PI - M_PI;
        char info[128];
        snprintf(info, sizeof(info), "Zoom: %d  Center: %.6f, %.6f PSX: %.6f, %.6f, %.6f",
                 zoom_level, lat, lon, Pos.latitude, Pos.longitude,
                 (float)Pos.heading * 180 / M_PI);
        DrawText(info, 10, 10, 16, BLACK);

        EndDrawing();
        UnloadRenderTexture(target);
    }

    // cleanup
    free_all_textures();
    curl_global_cleanup();
    CloseWindow();
    return 0;
}
