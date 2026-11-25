#ifndef CONFIG_H
#define CONFIG_H

/* KEY=VALUE text config (the SD card's cam/config.txt). Lines starting with
 * '#' are comments; CRLF line endings and surrounding quotes are accepted. */
typedef struct config config_t;

config_t   *config_load(const char *path);
const char *config_get(const config_t *c, const char *key, const char *def);
int         config_int(const config_t *c, const char *key, int def);
void        config_free(config_t *c);

#endif
