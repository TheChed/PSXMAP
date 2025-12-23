#include <stdint.h>



#define TILE_SIZE 256
#define CACHE_DIR "tilecache"
#define USER_AGENT "PSXMAP"
#define M_PI 3.14159265359
#define MAXBUFF 65536


    // Init raylib window
#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600

 struct pos{
    double latitude;
    double longitude;
    double heading;
} Pos;

typedef struct DLNode {
    char *key;
    struct DLNode *next;
} DLNode;

typedef struct TexNode {
    int z, x, y;
    Texture2D tex;
    struct TexNode *next;
} TexNode;


int socketID;
size_t bufmain_used = 0;
char bufmain[MAXBUFF];

void decode_pos(char *s);
