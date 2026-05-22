#ifndef ARCHIVE_RAR_PROFILE_PRIVATE_H_INCLUDED
#define ARCHIVE_RAR_PROFILE_PRIVATE_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct archive;

struct archive_rar_entry_profile
{
    int has_crc32;
    uint32_t crc32;

    int has_logical_crc32;
    uint32_t logical_crc32;

    int has_calculated_crc32;
    uint32_t calculated_crc32;

    int has_solid;
    int solid;

    int split_before;
    int split_after;

    int has_method;
    unsigned int method;
};

int archive_read_format_rar_current_entry_profile(struct archive* a, struct archive_rar_entry_profile* profile);
int archive_read_format_rar5_current_entry_profile(struct archive* a, struct archive_rar_entry_profile* profile);

#ifdef __cplusplus
}
#endif

#endif