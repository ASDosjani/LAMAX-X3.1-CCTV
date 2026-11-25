#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 128

struct config {
    int n;
    char *key[MAX_ENTRIES];
    char *val[MAX_ENTRIES];
};

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

config_t *config_load(const char *path)
{
    config_t *c = calloc(1, sizeof(*c));
    FILE *f = fopen(path, "r");
    if (!f) return c; /* empty config: every key falls back to its default */
    char line[512];
    while (fgets(line, sizeof(line), f) && c->n < MAX_ENTRIES) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
            v[vl - 1] = 0; v++;
        }
        for (char *p = k; *p; p++) *p = toupper((unsigned char)*p);
        c->key[c->n] = strdup(k);
        c->val[c->n] = strdup(v);
        c->n++;
    }
    fclose(f);
    return c;
}

const char *config_get(const config_t *c, const char *key, const char *def)
{
    for (int i = c->n - 1; i >= 0; i--)          /* last definition wins */
        if (!strcasecmp(c->key[i], key)) return c->val[i];
    return def;
}

int config_int(const config_t *c, const char *key, int def)
{
    const char *v = config_get(c, key, NULL);
    if (!v || !*v) return def;
    char *end;
    long r = strtol(v, &end, 10);
    return end == v ? def : (int)r;
}

void config_free(config_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->n; i++) { free(c->key[i]); free(c->val[i]); }
    free(c);
}
