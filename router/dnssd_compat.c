/*
 * dnssd_compat.c — 补齐 mdnsd 版 libdnssd.a 缺失的公共层函数
 *
 * UxPlay 的 mdnsd 后端(lib/mdnsd)只实现了后端钩子
 * (dnssd_private_init / dnssd_register_raop / dnssd_register_airplay)，
 * 但缺少公共层函数 dnssd_init / dnssd_set_airplay_features，
 * 以及 mdnsd 内部引用的 utils_hwaddr_raop / utils_hwaddr_airplay。
 * uxplay.exe 链接了 UxPlay/lib 主库(airplay)所以不缺；
 * router 只链接 libdnssd.a，需要在这里补齐这 4 个纯函数。
 * 实现复刻自 UxPlay/lib/dnssd.c 与 UxPlay/lib/utils.c 的对应部分。
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dnssd.h"

/* 与 UxPlay/lib/dnssdint.h 的默认值保持一致 */
#define DNSSD_COMPAT_FEATURES_1 0x5A7FFEE6UL
#define DNSSD_COMPAT_FEATURES_2 0x0UL

dnssd_t *dnssd_init(const char *name, int name_len, const char *hw_addr,
                    int hw_addr_len, unsigned char pin_pw, int *error)
{
    const char *dot_local = ".local";
    if (error) *error = DNSSD_ERROR_NOERROR;

    /* 校验 name 是 null 结尾且长度一致，且不是 ".local" */
    if (*(name + name_len) != '\0' || name_len != (int) strlen(name) ||
        !strcmp(name, dot_local)) {
        if (error) *error = DNSSD_ERROR_BADNAME;
        return NULL;
    }
    /* 只取 ".local" 之前的部分 */
    const char *dot_local_start = strstr(name, dot_local);
    if (dot_local_start) name_len = (int)(dot_local_start - name);

    dnssd_t *dnssd = (dnssd_t *) calloc(1, sizeof(dnssd_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    dnssd->pin_pw = pin_pw;
    dnssd->features1 = DNSSD_COMPAT_FEATURES_1;
    dnssd->features2 = DNSSD_COMPAT_FEATURES_2;

    dnssd->name_len = name_len;
    dnssd->name = calloc(1, name_len + 1);
    if (!dnssd->name) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->name, name, name_len);

    dnssd->hw_addr_len = hw_addr_len;
    dnssd->hw_addr = calloc(1, hw_addr_len);
    if (!dnssd->hw_addr) {
        free(dnssd->name);
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->hw_addr, hw_addr, hw_addr_len);

    dnssd->dnssd_private = dnssd_private_init(dnssd, error);
    if (!dnssd->dnssd_private) {
        free(dnssd->hw_addr);
        free(dnssd->name);
        free(dnssd);
        return NULL;
    }
    return dnssd;
}

void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val)
{
    uint32_t mask, *features;
    if (!dnssd || bit < 0 || bit > 63 || val < 0 || val > 1) return;
    if (bit >= 32) {
        mask = 0x1u << (bit - 32);
        features = &(dnssd->features2);
    } else {
        mask = 0x1u << bit;
        features = &(dnssd->features1);
    }
    if (val)
        *features |= mask;
    else
        *features &= ~mask;
}

int utils_hwaddr_raop(char *str, int str_len, const char *hwaddr, int hwaddrlen)
{
    if (str_len == 0 || str_len < 2 * hwaddrlen + 1)
        return -1;
    int j = 0;
    for (int i = 0; i < hwaddrlen; i++) {
        int hi = (hwaddr[i] >> 4) & 0x0f;
        int lo = hwaddr[i] & 0x0f;
        str[j++] = (hi < 10) ? '0' + hi : 'A' + hi - 10;
        str[j++] = (lo < 10) ? '0' + lo : 'A' + lo - 10;
    }
    str[j++] = '\0';
    return j;
}

int utils_hwaddr_airplay(char *str, int str_len, const char *hwaddr, int hwaddrlen)
{
    if (str_len == 0 || str_len < 2 * hwaddrlen + hwaddrlen)
        return -1;
    int j = 0;
    for (int i = 0; i < hwaddrlen; i++) {
        int hi = (hwaddr[i] >> 4) & 0x0f;
        int lo = hwaddr[i] & 0x0f;
        str[j++] = (hi < 10) ? '0' + hi : 'a' + hi - 10;
        str[j++] = (lo < 10) ? '0' + lo : 'a' + lo - 10;
        str[j++] = ':';
    }
    if (j != 0) j--;
    str[j++] = '\0';
    return j;
}
